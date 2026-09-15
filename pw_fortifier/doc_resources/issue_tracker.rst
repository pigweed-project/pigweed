---------------
1. IssueTracker
---------------
The first step is to implement an
:py:class:`~pw_fortifier.issue_tracker.IssueTracker` to manage communication
between the scanner and Buganizer.

1. Create a new file and define a subclass of ``IssueTracker``:

   .. code-block:: py

      from pw_fortifier.issue_tracker import IssueTracker


      class MyIssueTracker(IssueTracker):

          def __init__(self) -> None:
              super().__init__()

2. Set your project's Buganizer component ID (required):

   .. code-block:: py

      self.component_id = 1234567

3. Set the primary hotlist ID used for deduplication and tracking (required):

   .. code-block:: py

      self.primary_hotlist_id = 7654321

4. Set additional hotlists to attach to filed issues (optional):

   .. code-block:: py

      self.extra_hotlist_ids = [9876543]

5. Set the default assignee email address if no owner is found (required):

   .. code-block:: py

      self.default_assignee = 'sheriff@example.com'

6. Set default CC email addresses (optional):

   .. code-block:: py

      self.ccs = ['team-alerts@example.com']

7. Implement :py:meth:`~pw_fortifier.issue_tracker.IssueTracker.create` to file
   new issues. This is largely a passthrough to Buganizer APIs:

   .. code-block:: py

      async def create(self, issue: Issue) -> Issue:
          bug_id = await my_buganizer_api.create(
              component_id=self.component_id,
              title=issue.title,
              description=issue.description,
              assignee=issue.assignee or self.default_assignee,
              ccs=self.ccs,
              hotlists=self.hotlist_ids,
          )
          return issue._replace(issue_id=bug_id)

8. Implement :py:meth:`~pw_fortifier.issue_tracker.IssueTracker.read` to fetch
   an existing issue by ID:

   .. code-block:: py

      async def read(self, issue_id: int) -> Issue:
          record = await my_buganizer_api.query(issue=issue_id)
          return Issue(
              issue_id=record.id,
              title=record.title,
              description=record.description,
              assignee=record.assignee,
          )

9. Implement :py:meth:`~pw_fortifier.issue_tracker.IssueTracker.read_hotlist`
   to stream issues matching a hotlist ID:

   .. code-block:: py

      async def read_hotlist(self, hotlist_id: int) -> AsyncIterator[Issue]:
          records = await my_buganizer_api.query(hotlist=hotlist_id)
          for record in records:
              yield Issue(
                  issue_id=record.id,
                  title=record.title,
                  description=record.description,
                  assignee=record.assignee,
              )

For a complete working example, see
:cs:`pw_fortifier/py/pw_fortifier/demo_issue_tracker.py`.
